create table abc (var varchar(2));
insert into abc values('1');
insert into abc values('12');
insert into abc values('12');
insert into abc values('12');
insert into abc values('123');
insert into abc values('124');
insert into abc values('1234');
select * from abc;
insert into abc values(1234);
commit;
select * from abc;
